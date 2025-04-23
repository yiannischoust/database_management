#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf.h"
#include "bp_file.h"
#include "record.h"
#include "bp_datanode.h"
#include "bp_indexnode.h"
#include <stdbool.h>
#include <assert.h>

// Μακροεντολή για τον έλεγχο σφαλμάτων στις κλήσεις BF
#define CALL_BF(call)             \
    {                             \
        BF_ErrorCode code = call; \
        if (code != BF_OK)        \
        {                         \
            BF_PrintError(code);  \
            return BPLUS_ERROR;   \
        }                         \
    }

// Δηλώσεις συναρτήσεων για βασικές λειτουργίες του B+ δένδρου
int create_new_root(int, BPLUS_INFO *, int, int, int);
int split_parent(int, BPLUS_INFO *, int, BPLUS_INDEX_NODE *, int, int);
int insert_into_parent(int, BPLUS_INFO *, int, int, int, int);
int update_root(int, BPLUS_INFO *, int, int, int, int);
static int update_children_parents(int file_desc, BPLUS_INFO *bplus_info, int parentBlockNo, BPLUS_INDEX_NODE *parentNode);
static int update_indexnode_child_parent(int file_desc, BPLUS_INFO *bplus_info, int childBlockNo, int newParentBlockNo);

// Συνάρτηση δημιουργίας αρχείου B+ δένδρου
int BP_CreateFile(char *fileName)
{
    // Δημιουργία και αρχικοποίηση των απαιτούμενων blocks (header και root)
    BF_Block *headerblock;
    BF_Block *rootblock;
    BF_Block_Init(&headerblock);
    BF_Block_Init(&rootblock);
    int fileds;

    // Δημιουργία νέου αρχείου
    CALL_BF(BF_CreateFile(fileName));
    CALL_BF(BF_OpenFile(fileName, &fileds));
    CALL_BF(BF_AllocateBlock(fileds, headerblock));
    CALL_BF(BF_AllocateBlock(fileds, rootblock));

    // Αποθήκευση μεταδεδομένων στο header block
    BPLUS_INFO *metadata = (BPLUS_INFO *)BF_Block_GetData(headerblock);
    metadata->tree_depth = 1; // Αρχικό βάθος δένδρου
    metadata->file_name = fileName;
    metadata->max_recs_in_block = MAX_RECORDS_PER_BLOCK;
    metadata->max_pointers_in_block = MAX_POINTERS_PER_BLOCK;
    int block_no;
    BF_GetBlockCounter(fileds, &block_no);
    metadata->rootblock_no = block_no - 1; // Το root block είναι το τελευταίο

    // Αρχικοποίηση του root block
    BPLUS_INDEX_NODE *rootdata = (BPLUS_INDEX_NODE *)BF_Block_GetData(rootblock);
    rootdata->no_of_keys = 0;
    rootdata->previousblock = -1;
    rootdata->parent = -1;
    rootdata->is_innernode = 0; // Το root block ξεκινά ως φύλλο

    BF_Block_SetDirty(headerblock);
    BF_Block_SetDirty(rootblock);

    // Απελευθέρωση blocks
    CALL_BF(BF_UnpinBlock(headerblock));
    CALL_BF(BF_UnpinBlock(rootblock));
    BF_Block_Destroy(&headerblock);
    BF_Block_Destroy(&rootblock);

    // Κλείσιμο αρχείου
    CALL_BF(BF_CloseFile(fileds));

    return BPLUS_OK;
}

// Συνάρτηση ανοίγματος αρχείου B+ δένδρου
BPLUS_INFO *BP_OpenFile(char *fileName, int *file_desc)
{
    BPLUS_INFO *metadata;
    BF_OpenFile(fileName, file_desc); // Άνοιγμα αρχείου
    BF_Block *headerBlock;
    BF_Block_Init(&headerBlock);
    BF_GetBlock(*file_desc, 0, headerBlock); // Φόρτωση header block
    metadata = (BPLUS_INFO *)BF_Block_GetData(headerBlock); // Ανάκτηση μεταδεδομένων
    BF_UnpinBlock(headerBlock);
    BF_Block_Destroy(&headerBlock);
    return metadata;
}

// Συνάρτηση κλεισίματος αρχείου B+ δένδρου
int BP_CloseFile(int file_desc, BPLUS_INFO *info)
{
    BF_Block *headerBlock;
    BF_Block_Init(&headerBlock);
    BF_GetBlock(file_desc, 0, headerBlock); // Φόρτωση header block
    BPLUS_INFO *newmetadata = (BPLUS_INFO *)BF_Block_GetData(headerBlock);
    memcpy(newmetadata, info, sizeof(BPLUS_INFO)); // Ενημέρωση μεταδεδομένων
    BF_Block_SetDirty(headerBlock);
    CALL_BF(BF_UnpinBlock(headerBlock));
    BF_Block_Destroy(&headerBlock);
    CALL_BF(BF_CloseFile(file_desc)); // Κλείσιμο αρχείου
    return BPLUS_OK;
}

// Συνάρτηση εισαγωγής εγγραφής στο B+ δένδρο
int BP_InsertEntry(int file_desc, BPLUS_INFO *bplus_info, Record record)
{
    BF_Block *currentBlock;
    BF_Block_Init(&currentBlock);
    int currentBlockNo = bplus_info->rootblock_no; // Ξεκινάμε από το root block

    // Έλεγχος αν η εγγραφή υπάρχει ήδη
    if (BP_GetEntry(file_desc, bplus_info, record.id, NULL) == BPLUS_OK)
    {
        return BPLUS_ERROR; // Επιστροφή σφάλματος σε περίπτωση διπλής εγγραφής
    }

    // Πλοήγηση στο B+ δένδρο μέχρι να φτάσουμε στο σωστό φύλλο
    while (1)
    {
        CALL_BF(BF_GetBlock(file_desc, currentBlockNo, currentBlock));
        char *blockData = BF_Block_GetData(currentBlock);
        BPLUS_INDEX_NODE *testNode = (BPLUS_INDEX_NODE *)blockData;
        
        if (testNode->is_innernode == 0)
        {
            // Είμαστε σε φύλλο
            BPLUS_DATA_NODE *currentDataNode = (BPLUS_DATA_NODE *)blockData;

            // Εισαγωγή εγγραφής στο φύλλο
            if (bplus_data_node_insert(bplus_info, currentBlockNo, currentDataNode, record) == 0)
            { 
                BF_Block_SetDirty(currentBlock);
                break;
            }
            else
            {
                // Διαχωρισμός φύλλου λόγω πληρότητας
                Record records[MAX_RECORDS_PER_BLOCK + 1] = {0};

                for (int i = 0; i < MAX_RECORDS_PER_BLOCK; i++)
                {
                    records[i] = currentDataNode->allrecords[i];
                }

                int insertPos = 0;
                while (insertPos < MAX_RECORDS_PER_BLOCK && records[insertPos].id < record.id)
                {
                    insertPos++;
                }

                for (int i = MAX_RECORDS_PER_BLOCK; i > insertPos; i--)
                {
                    records[i] = records[i - 1];
                }

                records[insertPos] = record;

                int mid = records[(MAX_RECORDS_PER_BLOCK + 1) / 2].id;

                // Δημιουργία νέου block για διαχωρισμένο φύλλο
                BF_Block *newDataBlock;
                BF_Block_Init(&newDataBlock);
                CALL_BF(BF_AllocateBlock(file_desc, newDataBlock));

                BPLUS_DATA_NODE *newDataNode = (BPLUS_DATA_NODE *)BF_Block_GetData(newDataBlock);
                int newDataBlockNo;
                CALL_BF(BF_GetBlockCounter(file_desc, &newDataBlockNo));
                newDataBlockNo -= 1;
                newDataNode->is_innernode = 0;
                newDataNode->no_of_records = 0;
                newDataNode->parent = currentDataNode->parent;
                newDataNode->nextdatanode = currentDataNode->nextdatanode;

                currentDataNode->no_of_records = 0;
                currentDataNode->nextdatanode = newDataBlockNo;

                for (int i = 0; i < MAX_RECORDS_PER_BLOCK + 1; i++)
                {
                    Record rec = records[i];
                    if (rec.id <= mid)
                    {
                        bplus_data_node_insert(bplus_info, currentBlockNo, currentDataNode, rec);
                    }
                    else
                    {
                        bplus_data_node_insert(bplus_info, newDataBlockNo, newDataNode, rec);
                    }
                }

                // Ενημέρωση γονικού block ή δημιουργία νέας ρίζας
                if (currentDataNode->parent == -1)
                {
                    int newRootBlockNo = create_new_root(file_desc, bplus_info, currentBlockNo, mid, newDataBlockNo);
                    newDataNode->parent = newRootBlockNo;
                    currentDataNode->parent = newRootBlockNo;
                }
                else
                {
                    insert_into_parent(file_desc, bplus_info, currentDataNode->parent, currentBlockNo, mid, newDataBlockNo);
                }

                BF_Block_SetDirty(currentBlock);
                BF_Block_SetDirty(newDataBlock);
                CALL_BF(BF_UnpinBlock(newDataBlock));
                BF_Block_Destroy(&newDataBlock);

                break;
            }
        }
        else
        {
            // Είμαστε σε εσωτερικό κόμβο, συνεχίζουμε την πλοήγηση
            BPLUS_INDEX_NODE *currentIndexNode = (BPLUS_INDEX_NODE *)blockData;

            if (record.id <= currentIndexNode->bplusstructure[0].id)
            {
                currentBlockNo = currentIndexNode->previousblock;
            }
            else
            {
                int found = 0;

                for (int i = 1; i < currentIndexNode->no_of_keys; i++)
                {
                    if (record.id <= currentIndexNode->bplusstructure[i].id)
                    {
                        currentBlockNo = currentIndexNode->bplusstructure[i - 1].blockno;
                        found = 1;
                        break;
                    }
                }

                if (!found)
                {
                    currentBlockNo = currentIndexNode->bplusstructure[currentIndexNode->no_of_keys - 1].blockno;
                }
            }

            CALL_BF(BF_UnpinBlock(currentBlock));
        }
    }

    CALL_BF(BF_UnpinBlock(currentBlock));
    BF_Block_Destroy(&currentBlock);

    return BPLUS_OK;
}
int insert_into_parent(int file_desc, BPLUS_INFO *bplus_info, int parentBlockNo, int leftBlockNo, int key, int rightBlockNo)
{
    // Συνάρτηση εισαγωγής ενός νέου κλειδιού και του αντίστοιχου δείκτη στον γονικό κόμβο

    BF_Block *pBlock = NULL;
    BF_Block_Init(&pBlock);

    CALL_BF(BF_GetBlock(file_desc, parentBlockNo, pBlock));

    BPLUS_INDEX_NODE *parentNode = (BPLUS_INDEX_NODE *)BF_Block_GetData(pBlock);

    // Εύρεση της κατάλληλης θέσης για το νέο κλειδί
    int insertPos = 0;
    while (insertPos < parentNode->no_of_keys && parentNode->bplusstructure[insertPos].id < key)
    {
        insertPos++;
    }

    // Εισαγωγή του κλειδιού εάν υπάρχει χώρος στον γονικό κόμβο
    if (parentNode->no_of_keys < bplus_info->max_pointers_in_block)
    {
        for (int i = parentNode->no_of_keys; i > insertPos; i--)
        {
            parentNode->bplusstructure[i] = parentNode->bplusstructure[i - 1];
        }
        parentNode->bplusstructure[insertPos].id = key;
        parentNode->bplusstructure[insertPos].blockno = rightBlockNo;
        parentNode->no_of_keys++;

        BF_Block_SetDirty(pBlock);

        CALL_BF(BF_UnpinBlock(pBlock));
        BF_Block_Destroy(&pBlock);

        return BPLUS_OK;
    }

    // Εάν δεν υπάρχει χώρος, επιστρέφει σφάλμα
    CALL_BF(BF_UnpinBlock(pBlock));
    BF_Block_Destroy(&pBlock);

    return BPLUS_ERROR;
}

int update_root(int file_desc, BPLUS_INFO *bplus_info, int leftBlockNo, int key, int rightBlockNo, int RootBlockNo)
{
    // Συνάρτηση ενημέρωσης της ρίζας του δένδρου όταν γίνεται split σε επίπεδο ρίζας

    BF_Block *RootBlock;
    BF_Block_Init(&RootBlock);
    BF_GetBlock(file_desc, RootBlockNo, RootBlock);

    BPLUS_INDEX_NODE *newRootNode = (BPLUS_INDEX_NODE *)BF_Block_GetData(RootBlock);

    // Εισαγωγή του νέου κλειδιού και των δεικτών στη ρίζα
    bplus_index_node_insert(bplus_info, RootBlockNo, newRootNode, key, leftBlockNo, rightBlockNo);

    // Ενημέρωση του γονικού δείκτη για τα παιδιά
    update_indexnode_child_parent(file_desc, bplus_info, leftBlockNo, RootBlockNo);
    update_indexnode_child_parent(file_desc, bplus_info, rightBlockNo, RootBlockNo);

    BF_Block_SetDirty(RootBlock);
    CALL_BF(BF_UnpinBlock(RootBlock));
    BF_Block_Destroy(&RootBlock);

    return BPLUS_OK;
}

int create_new_root(int file_desc, BPLUS_INFO *bplus_info, int leftBlockNo, int key, int rightBlockNo)
{
    // Συνάρτηση δημιουργίας νέας ρίζας όταν γίνεται διαχωρισμός του root block

    BF_Block *newRootBlock;
    BF_Block_Init(&newRootBlock);
    CALL_BF(BF_AllocateBlock(file_desc, newRootBlock));

    BPLUS_INDEX_NODE *newRootNode = (BPLUS_INDEX_NODE *)BF_Block_GetData(newRootBlock);
    newRootNode->no_of_keys = 1;
    newRootNode->parent = -1; // Η νέα ρίζα δεν έχει γονικό κόμβο
    newRootNode->bplusstructure[0].id = key;
    newRootNode->bplusstructure[0].blockno = rightBlockNo;
    newRootNode->previousblock = leftBlockNo;
    newRootNode->is_innernode = 1;

    int newRootBlockNo;
    BF_GetBlockCounter(file_desc, &newRootBlockNo);
    newRootBlockNo -= 1;
    bplus_info->rootblock_no = newRootBlockNo;

    BF_Block_SetDirty(newRootBlock);
    CALL_BF(BF_UnpinBlock(newRootBlock));
    BF_Block_Destroy(&newRootBlock);

    return newRootBlockNo;
}

int BP_GetEntry(int file_desc, BPLUS_INFO *bplus_info, int value, Record **record)
{
    // Συνάρτηση εύρεσης μιας εγγραφής στο B+ δένδρο με βάση το ID

    BF_Block *block = NULL;
    BF_Block_Init(&block);

    int currentBlockNo = bplus_info->rootblock_no;

    while (1)
    {
        CALL_BF(BF_GetBlock(file_desc, currentBlockNo, block));

        void *blockData = BF_Block_GetData(block);

        BPLUS_INDEX_NODE *testNode = (BPLUS_INDEX_NODE *)blockData;

        // Αν ο κόμβος είναι εσωτερικός, συνεχίζουμε την πλοήγηση
        if (testNode->is_innernode == 1)
        {
            BPLUS_INDEX_NODE *currentIndexNode = (BPLUS_INDEX_NODE *)blockData;

            if (value <= currentIndexNode->bplusstructure[0].id)
            {
                currentBlockNo = currentIndexNode->previousblock;
            }
            else
            {
                int found = 0;

                for (int i = 1; i < currentIndexNode->no_of_keys; i++)
                {
                    if (value <= currentIndexNode->bplusstructure[i].id)
                    {
                        currentBlockNo = currentIndexNode->bplusstructure[i - 1].blockno;
                        found = 1;
                        break;
                    }
                }

                if (found == 0)
                {
                    currentBlockNo = currentIndexNode->bplusstructure[currentIndexNode->no_of_keys - 1].blockno;
                }
            }
        }
        else
        {
            // Αν ο κόμβος είναι φύλλο, αναζητούμε την εγγραφή
            BPLUS_DATA_NODE *currentDataNode = (BPLUS_DATA_NODE *)blockData;
            for (int i = 0; i < currentDataNode->no_of_records; i++)
            {
                if (currentDataNode->allrecords[i].id == value)
                {
                    *record = malloc(sizeof(Record));

                    if (*record == NULL)
                    {
                        printf("Error: Could not allocate memory for result record.\n");
                        exit(1);
                    }

                    memcpy(*record, &currentDataNode->allrecords[i], sizeof(Record));

                    CALL_BF(BF_UnpinBlock(block));
                    BF_Block_Destroy(&block);

                    return BPLUS_OK;
                }
            }

            CALL_BF(BF_UnpinBlock(block));

            break;
        }

        CALL_BF(BF_UnpinBlock(block));
    }

    BF_Block_Destroy(&block);

    return BPLUS_ERROR;
}

static int update_children_parents(int file_desc, BPLUS_INFO *bplus_info, int parentBlockNo, BPLUS_INDEX_NODE *parentNode)
{
    // Συνάρτηση ενημέρωσης του γονικού κόμβου για όλους τους δείκτες του

    if (parentNode->previousblock != -1)
    {
        update_indexnode_child_parent(file_desc, bplus_info, parentNode->previousblock, parentBlockNo);
    }

    for (int i = 0; i < parentNode->no_of_keys; i++)
    {
        update_indexnode_child_parent(file_desc, bplus_info, parentNode->bplusstructure[i].blockno, parentBlockNo);
    }

    return 0;
}

static int update_indexnode_child_parent(int file_desc, BPLUS_INFO *bplus_info, int childBlockNo, int newParentBlockNo)
{
    // Συνάρτηση ενημέρωσης του γονικού δείκτη ενός παιδικού κόμβου

    BF_Block *cBlock;
    BF_Block_Init(&cBlock);
    CALL_BF(BF_GetBlock(file_desc, childBlockNo, cBlock));
    void *cData = BF_Block_GetData(cBlock);

    BPLUS_DATA_NODE *leafCheck = (BPLUS_DATA_NODE *)cData;
    if (leafCheck->no_of_records >= 0 && leafCheck->no_of_records <= bplus_info->max_recs_in_block)
    {
        leafCheck->parent = newParentBlockNo;
    }
    else
    {
        ((BPLUS_INDEX_NODE *)cData)->parent = newParentBlockNo;
    }

    BF_Block_SetDirty(cBlock);
    CALL_BF(BF_UnpinBlock(cBlock));
    BF_Block_Destroy(&cBlock);

    return 0;
}

int BP_Print(int file_desc, BPLUS_INFO *bplus_info)
{
    // Συνάρτηση εκτύπωσης όλων των κόμβων του δένδρου

    BF_Block *currentBlock;
    BF_Block_Init(&currentBlock);

    int total_blocks;
    CALL_BF(BF_GetBlockCounter(file_desc, &total_blocks));

    for (int currentBlockNo = 1; currentBlockNo < total_blocks; currentBlockNo++)
    {
        printf("====================================================\n");

        CALL_BF(BF_GetBlock(file_desc, currentBlockNo, currentBlock));

        BPLUS_INDEX_NODE *testNode = (BPLUS_INDEX_NODE *)BF_Block_GetData(currentBlock);

        if (testNode->is_innernode == 1)
        {
            bplus_inner_node_print(currentBlockNo, testNode);
        }
        else
        {
            BPLUS_DATA_NODE *blockData = (BPLUS_DATA_NODE *)BF_Block_GetData(currentBlock);
            bplus_data_node_print(currentBlockNo, blockData);
        }

        CALL_BF(BF_UnpinBlock(currentBlock));
    }

    printf("====================================================\n");

    BF_Block_Destroy(&currentBlock);

    return total_blocks;
}
